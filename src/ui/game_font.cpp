#include "game_font.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <fstream>
#include <iterator>
#include <unordered_map>
#include <string_view>
#include <vector>

#include <imgui.h>
#include <imgui_internal.h>
#include <rex/logging.h>

namespace asura::ui {
namespace {

struct Glyph {
  uint16_t page, x, y, w, h;
  int16_t y_offset;
  int advance = 0;  // font pixels
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
  for (auto& [codepoint, g] : font.glyphs) {
    if (g.page >= font.pages.size() || g.x + g.w > font.pages[g.page].width ||
        g.y + g.h > font.pages[g.page].height) {
      return false;
    }
    // The advance is the bitmap width plus the font's kerning (-2 in the
    // game's fonts), which overlaps the padding on the right of Latin glyphs.
    // Japanese glyphs are cropped to their ink, so the same overlap makes
    // them touch (イ against ト): they keep their full width.
    g.advance = g.w + (codepoint >= 0x2E80 ? std::max<int>(font.kerning, 0) : font.kerning);
  }
  return font.line_height > 0;
}

// Zero-terminated [first, last] pairs covering the font's codepoints; kept
// for the atlas' lifetime (ImFontConfig::GlyphRanges is not copied).
const ImWchar* GlyphRangesOf(const FontData& font) {
  static std::deque<std::vector<ImWchar>> storage;
  std::vector<uint32_t> codepoints;
  for (const auto& [codepoint, glyph] : font.glyphs) {
    if (codepoint > 0 && codepoint <= IM_UNICODE_CODEPOINT_MAX) codepoints.push_back(codepoint);
  }
  std::sort(codepoints.begin(), codepoints.end());
  std::vector<ImWchar>& ranges = storage.emplace_back();
  for (size_t i = 0; i < codepoints.size();) {
    size_t j = i;
    while (j + 1 < codepoints.size() && codepoints[j + 1] == codepoints[j] + 1) ++j;
    ranges.push_back(ImWchar(codepoints[i]));
    ranges.push_back(ImWchar(codepoints[j]));
    i = j + 1;
  }
  ranges.push_back(0);
  return ranges.data();
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
  const float advance = std::max(0.0f, float(g.advance) * scale);
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

std::vector<std::filesystem::path> GameUiDirs() {
  std::vector<std::filesystem::path> dirs;
  if (const char* dir = std::getenv("ASURA_UI_DIR"); dir && *dir) dirs.emplace_back(dir);
  if (const char* xdg = std::getenv("XDG_CACHE_HOME"); xdg && *xdg) {
    dirs.push_back(std::filesystem::path(xdg) / "asura_wrath_recomp" / "ui");
  }
  if (const char* home = std::getenv("HOME"); home && *home) {
    dirs.push_back(std::filesystem::path(home) / ".cache" / "asura_wrath_recomp" / "ui");
  }
  std::error_code ec;
  const auto exe = std::filesystem::read_symlink("/proc/self/exe", ec);
  if (!ec) {
    // The repository's out/assets/ui (extracted game files), from out/build/<preset>/.
    dirs.push_back(exe.parent_path().parent_path().parent_path() / "assets" / "ui");
    // The project's own art (assets/ui), staged next to the executable at build time.
    dirs.push_back(exe.parent_path() / "assets" / "ui");
  }
  return dirs;
}

std::filesystem::path GameUiFile(std::string_view name) {
  const auto dirs = GameUiDirs();
  std::error_code ec;
  for (const auto& dir : dirs) {
    if (std::filesystem::is_regular_file(dir / name, ec)) return dir / name;
  }
  return dirs.empty() ? std::filesystem::path(name) : dirs.front() / name;
}

ImFont* AddGameFont(ImFontAtlas* atlas, const std::filesystem::path& path, ImFont* merge_into) {
  std::ifstream file(path, std::ios::binary);
  if (!file) {
    if (!merge_into) REXLOG_INFO("UI font {} not found", path.string());
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
  config.MergeMode = merge_into != nullptr;
  config.DstFont = merge_into;
  // The SDK uploads the atlas once (ImGuiDrawer: GetTexDataAsRGBA32), so only
  // the glyphs in GlyphRanges are ever baked: list all of the file's
  // (the default is Latin-1 only, which drops e.g. Japanese).
  config.GlyphRanges = GlyphRangesOf(check);
  std::snprintf(config.Name, sizeof(config.Name), "%s", path.stem().string().c_str());
  ImFont* font = atlas->AddFont(&config);
  REXLOG_INFO("UI font {} {}", path.string(), font ? "loaded" : "failed to load");
  return font;
}

void AddFallbackFonts(ImFontAtlas* atlas, ImFont* font) {
  if (!font) return;
  // The game's fonts lack a few ASCII characters (# $ @ ^ { | } and more in
  // TitleFont): take those from ImGui's default font.
  ImFontConfig fallback;
  fallback.MergeMode = true;
  fallback.DstFont = font;
  atlas->AddFontDefault(&fallback);
}

ImFont* AddBundledFont(ImFontAtlas* atlas, bool bold) {
  const auto path = GameUiFile(bold ? "NotoSerif-Bold.ttf" : "NotoSerif-Regular.ttf");
  std::error_code ec;
  if (!std::filesystem::is_regular_file(path, ec)) {
    REXLOG_INFO("UI font {} not found", path.string());
    return nullptr;
  }
  // Latin, Latin-1 and Extended-A (the European languages), general
  // punctuation, the euro and trademark signs. Baked once (the SDK's ImGui
  // backend has no dynamic atlas) at a size that scales down cleanly.
  static const ImWchar kRanges[] = {0x0020, 0x024F, 0x2000, 0x206F, 0x20AC, 0x20AC,
                                    0x2122, 0x2122, 0};
  ImFontConfig config;
  config.OversampleH = 2;
  std::snprintf(config.Name, sizeof(config.Name), "%s", path.stem().string().c_str());
  ImFont* font = atlas->AddFontFromFileTTF(path.string().c_str(), 40.0f, &config, kRanges);
  REXLOG_INFO("UI font {} {}", path.string(), font ? "loaded" : "failed to load");
  if (font) AddGameFont(atlas, GameUiFile("NotoSerifJP-Launcher.awf"), font);
  return font;
}

ImFont* AddGameFontFamily(ImFontAtlas* atlas, std::string_view name) {
  const std::string base(name);
  ImFont* font = AddGameFont(atlas, GameUiFile(base + ".awf"));
  if (!font) return nullptr;
  // Accents (the European packages' fonts) and Japanese (the JPN one) for
  // the glyphs the international font lacks; same typeface.
  for (const char* language : kGameFontLanguages) {
    AddGameFont(atlas, GameUiFile(base + "_" + language + ".awf"), font);
  }
  AddFallbackFonts(atlas, font);
  return font;
}

}  // namespace asura::ui
