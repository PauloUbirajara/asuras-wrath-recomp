#include "game_assets.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <map>
#include <optional>
#include <vector>

#include <rex/system/lzx.h>
#include <rex/system/util/xdbf_utils.h>

#include "game_font.h"
#include "translation.h"
#include "xxx_package.h"  // tools/mantra_shadergen: .xxx (UE3 + LZX) decompression
#include "../platform/platform.h"

// AES-128-CBC with a zero IV, from the SDK's XEX loader (librexruntime).
void aes_decrypt_buffer(const uint8_t* session_key, const uint8_t* input_buffer,
                        const size_t input_size, uint8_t* output_buffer, const size_t output_size);

namespace asura::ui {

namespace fs = std::filesystem;

namespace {

using Bytes = std::vector<uint8_t>;

bool ReadFile(const fs::path& path, Bytes& out) {
  std::ifstream file(path, std::ios::binary);
  if (!file) return false;
  out.assign(std::istreambuf_iterator<char>(file), {});
  return true;
}

bool WriteFile(const fs::path& path, const void* data, size_t size) {
  std::ofstream file(path, std::ios::binary | std::ios::trunc);
  file.write(static_cast<const char*>(data), std::streamsize(size));
  return bool(file);
}

uint32_t Be32(const uint8_t* p) {
  return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3];
}
uint16_t Be16(const uint8_t* p) { return uint16_t(p[0] << 8 | p[1]); }

void Fail(std::string* error, std::string text) {
  if (error) *error = std::move(text);
}

// --- XEX2 -----------------------------------------------------------------------
// Only what reading a resource needs: decrypt and decompress the image (as
// rex::runtime::XexModule::ReadImage does, without guest memory).

constexpr uint8_t kRetailKey[16] = {0x20, 0xB1, 0x85, 0xA5, 0x9D, 0x28, 0xFD, 0xC3,
                                    0x40, 0x58, 0x3F, 0xBB, 0x08, 0x96, 0xBF, 0x91};

struct XexImage {
  Bytes image;
  uint32_t base = 0;
  uint32_t resource_info = 0;  // offset in the file of the resource info header
};

bool LoadXex(const Bytes& xex, XexImage& out, std::string* error) {
  if (xex.size() < 24 || std::memcmp(xex.data(), "XEX2", 4) != 0) {
    Fail(error, "default.xex is not an XEX2 file");
    return false;
  }
  const uint32_t header_size = Be32(&xex[8]), security = Be32(&xex[16]), count = Be32(&xex[20]);
  if (header_size > xex.size() || 24 + size_t(count) * 8 > xex.size() ||
      security + 0x160 > xex.size()) {
    Fail(error, "default.xex header is damaged");
    return false;
  }
  uint32_t file_format = 0;
  for (uint32_t i = 0; i < count; ++i) {
    const uint32_t key = Be32(&xex[24 + i * 8]), value = Be32(&xex[28 + i * 8]);
    if (key == 0x000003FF) file_format = value;
    if (key == 0x00010201) out.base = value;
    if (key == 0x000002FF) out.resource_info = value;
  }
  const uint32_t image_size = Be32(&xex[security + 4]);
  if (!out.base) out.base = Be32(&xex[security + 0x110]);
  if (!file_format || file_format + 12 > xex.size() || !out.resource_info) {
    Fail(error, "default.xex has no file format or resource info");
    return false;
  }
  uint8_t session_key[16];
  aes_decrypt_buffer(kRetailKey, &xex[security + 0x150], 16, session_key, 16);

  const uint32_t info_size = Be32(&xex[file_format]);
  const uint16_t encryption = Be16(&xex[file_format + 4]);
  const uint16_t compression = Be16(&xex[file_format + 6]);
  // The data, decrypted in one CBC pass (blocks are stored back to back).
  Bytes data(xex.begin() + header_size, xex.end());
  data.resize((data.size() + 15) & ~size_t(15));
  if (encryption == 1) {
    Bytes plain(data.size());
    aes_decrypt_buffer(session_key, data.data(), data.size(), plain.data(), plain.size());
    data.swap(plain);
  } else if (encryption != 0) {
    Fail(error, "default.xex uses an unknown encryption");
    return false;
  }

  out.image.assign(image_size, 0);
  if (compression == 0) {
    std::copy_n(data.begin(), std::min<size_t>(data.size(), image_size), out.image.begin());
  } else if (compression == 1) {
    // Basic: (data size, zero size) blocks.
    size_t src = 0, dst = 0;
    for (uint32_t b = 0; b < (info_size - 8) / 8; ++b) {
      const uint32_t data_size = Be32(&xex[file_format + 8 + b * 8]);
      const uint32_t zero_size = Be32(&xex[file_format + 12 + b * 8]);
      if (src + data_size > data.size() || dst + data_size > out.image.size()) break;
      std::copy_n(data.begin() + src, data_size, out.image.begin() + dst);
      src += data_size;
      dst += data_size + zero_size;
    }
  } else if (compression == 2) {
    // Normal: a chain of [u32 next size][20 B hash][u16-sized chunks] blocks
    // holding one LZX stream.
    const uint32_t window = Be32(&xex[file_format + 8]);
    uint32_t block_size = Be32(&xex[file_format + 12]);
    Bytes lzx;
    size_t p = 0;
    while (block_size && p + block_size <= data.size()) {
      const size_t next = p + block_size;
      const uint32_t next_size = Be32(&data[p]);
      size_t q = p + 24;
      while (q + 2 <= next) {
        const size_t chunk = Be16(&data[q]);
        q += 2;
        if (!chunk || q + chunk > next) break;
        lzx.insert(lzx.end(), data.begin() + q, data.begin() + q + chunk);
        q += chunk;
      }
      p = next;
      block_size = next_size;
    }
    if (lzx_decompress(lzx.data(), lzx.size(), out.image.data(), out.image.size(), window,
                       nullptr, 0) != 0) {
      Fail(error, "default.xex did not decompress");
      return false;
    }
  } else {
    Fail(error, "default.xex uses an unknown compression");
    return false;
  }
  return true;
}

// The resource named `name` (e.g. the title id) inside the loaded image.
std::optional<std::pair<const uint8_t*, size_t>> XexResource(const Bytes& xex,
                                                             const XexImage& image,
                                                             const char* name) {
  const uint32_t info = image.resource_info;
  if (info + 4 > xex.size()) return std::nullopt;
  const uint32_t size = Be32(&xex[info]);
  for (uint32_t at = info + 4; at + 16 <= info + size && at + 16 <= xex.size(); at += 16) {
    if (std::strncmp(reinterpret_cast<const char*>(&xex[at]), name, 8) != 0) continue;
    const uint32_t address = Be32(&xex[at + 8]), length = Be32(&xex[at + 12]);
    if (address < image.base || address - image.base + size_t(length) > image.image.size()) {
      return std::nullopt;
    }
    return std::make_pair(image.image.data() + (address - image.base), size_t(length));
  }
  return std::nullopt;
}

// --- UE3 package (v841, big-endian) ------------------------------------------------
// A port of the parts of tools/asset_extract.py the fonts need.

struct Reader {
  const Bytes& d;
  size_t p = 0;
  bool ok = true;
  bool Need(size_t n) {
    if (p + n > d.size()) ok = false;
    return ok;
  }
  uint32_t U32() {
    if (!Need(4)) return 0;
    const uint32_t v = Be32(&d[p]);
    p += 4;
    return v;
  }
  int32_t I32() { return int32_t(U32()); }
  uint8_t U8() { return Need(1) ? d[p++] : 0; }
  std::string FString() {
    const int32_t n = I32();
    std::string s;
    if (n > 0 && Need(size_t(n))) {
      s.assign(reinterpret_cast<const char*>(&d[p]), size_t(n));
      p += size_t(n);
    } else if (n < 0 && Need(size_t(-n) * 2)) {
      for (int32_t i = 0; i < -n; ++i) s += char(d[p + size_t(i) * 2 + 1]);  // ASCII names only
      p += size_t(-n) * 2;
    }
    while (!s.empty() && s.back() == '\0') s.pop_back();
    return s;
  }
};

struct Export {
  int32_t class_index = 0, outer = 0;
  std::string name;
  int32_t size = 0, offset = 0;
};

struct Package {
  Bytes data;
  std::vector<std::string> names;
  std::vector<std::string> import_names;
  std::vector<Export> exports;

  std::string Name(int32_t index, int32_t number) const {
    std::string n = index >= 0 && size_t(index) < names.size() ? names[index] : "?";
    return number ? n + "_" + std::to_string(number - 1) : n;
  }
  std::string ObjectName(int32_t index) const {
    if (index > 0 && size_t(index) <= exports.size()) return exports[index - 1].name;
    if (index < 0 && size_t(-index) <= import_names.size()) return import_names[-index - 1];
    return "Class";
  }
  std::string FullPath(const Export& e) const {
    std::string path = e.name;
    for (int32_t outer = e.outer; outer > 0 && size_t(outer) <= exports.size();
         outer = exports[outer - 1].outer) {
      path = exports[outer - 1].name + "." + path;
    }
    return path;
  }
  Bytes Blob(const Export& e) const {
    if (e.offset < 0 || e.size < 0 || size_t(e.offset) + size_t(e.size) > data.size()) return {};
    return Bytes(data.begin() + e.offset, data.begin() + e.offset + e.size);
  }
};

bool ParsePackage(Package& pkg) {
  Reader r{pkg.data};
  if (r.U32() != 0x9E2A83C1) return false;
  r.U32();  // version
  r.I32();  // header size
  r.FString();
  r.U32();  // flags
  const int32_t name_count = r.I32(), name_offset = r.I32();
  const int32_t export_count = r.I32(), export_offset = r.I32();
  const int32_t import_count = r.I32(), import_offset = r.I32();
  if (!r.ok || name_count < 0 || export_count < 0 || import_count < 0) return false;
  r.p = size_t(name_offset);
  for (int32_t i = 0; i < name_count && r.ok; ++i) {
    pkg.names.push_back(r.FString());
    r.p += 8;
  }
  r.p = size_t(import_offset);
  for (int32_t i = 0; i < import_count && r.ok; ++i) {
    r.p += 16;  // class package, class
    r.I32();    // outer
    const int32_t a = r.I32(), b = r.I32();
    pkg.import_names.push_back(pkg.Name(a, b));
  }
  r.p = size_t(export_offset);
  for (int32_t i = 0; i < export_count && r.ok; ++i) {
    Export e;
    e.class_index = r.I32();
    r.I32();  // super
    e.outer = r.I32();
    const int32_t a = r.I32(), b = r.I32();
    e.name = pkg.Name(a, b);
    r.I32();      // archetype
    r.p += 8;     // object flags
    e.size = r.I32();
    e.offset = r.I32();
    r.U32();      // export flags
    const int32_t net = r.I32();
    r.p += size_t(std::max(net, 0)) * 4 + 16 + 4;
    pkg.exports.push_back(std::move(e));
  }
  return r.ok;
}

struct Property {
  std::string type;
  Bytes raw;
  bool flag = false;
};

// Tagged properties from `pos`; `end` is where they stop.
std::map<std::string, Property> ReadProperties(const Package& pkg, const Bytes& data, size_t pos,
                                               size_t* end) {
  std::map<std::string, Property> props;
  Reader r{data, pos};
  for (int guard = 0; guard < 512 && r.ok; ++guard) {
    int32_t a = r.I32(), b = r.I32();
    const std::string name = pkg.Name(a, b);
    if (name == "None") break;
    a = r.I32();
    b = r.I32();
    Property prop;
    prop.type = pkg.Name(a, b);
    const int32_t size = r.I32(), index = r.I32();
    if (prop.type == "BoolProperty") {
      prop.flag = r.U8() != 0;
    } else {
      if (prop.type == "StructProperty" || prop.type == "ByteProperty") r.p += 8;
      if (size < 0 || !r.Need(size_t(size))) break;
      prop.raw.assign(data.begin() + r.p, data.begin() + r.p + size);
      r.p += size_t(size);
    }
    props[index ? name + "[" + std::to_string(index) + "]" : name] = std::move(prop);
  }
  if (end) *end = r.p;
  return props;
}

int32_t PropInt(const std::map<std::string, Property>& props, const char* name, int32_t def = 0) {
  const auto it = props.find(name);
  return it != props.end() && it->second.raw.size() == 4 ? int32_t(Be32(it->second.raw.data()))
                                                          : def;
}
float PropFloat(const std::map<std::string, Property>& props, const char* name) {
  const uint32_t bits = uint32_t(PropInt(props, name));
  float f;
  std::memcpy(&f, &bits, 4);
  return f;
}
std::string PropName(const Package& pkg, const std::map<std::string, Property>& props,
                     const char* name) {
  const auto it = props.find(name);
  if (it == props.end() || it->second.raw.size() != 8) return {};
  return pkg.Name(int32_t(Be32(it->second.raw.data())), int32_t(Be32(it->second.raw.data() + 4)));
}

// Xbox 360 tiled byte offset of block (x, y); pitch in blocks (32-aligned).
uint32_t TiledOffset(uint32_t x, uint32_t y, uint32_t pitch, uint32_t log_bpb) {
  const uint32_t macro = ((x >> 5) + (y >> 5) * (pitch >> 5)) << (log_bpb + 7);
  const uint32_t micro = ((x & 7) + ((y & 0xE) << 2)) << log_bpb;
  const uint32_t offset = macro + ((micro & ~0xFu) << 1) + (micro & 0xF) + ((y & 1) << 4);
  return ((offset & ~0x1FFu) << 3) + ((y & 16) << 7) + ((offset & 0x1C0) << 2) +
         (((((y & 8) >> 2) + (x >> 3)) & 3) << 6) + (offset & 0x3F);
}

// A DXT5 Texture2D's alpha channel (mip 0), as width x height bytes.
bool DecodeDxt5Alpha(const Package& pkg, const Export& e, uint16_t& width, uint16_t& height,
                     Bytes& alpha) {
  const Bytes data = pkg.Blob(e);
  if (data.size() < 8) return false;
  size_t end = 0;
  const auto props = ReadProperties(pkg, data, 4, &end);
  if (PropName(pkg, props, "Format") != "PF_DXT5") return false;
  Reader r{data, end};
  auto read_bulk = [&](Bytes* out) {
    const uint32_t flags = r.U32();
    r.I32();  // element count
    const int32_t size = r.I32();
    r.I32();  // offset
    if ((flags & 0x21) || size <= 0) return false;
    if (!r.Need(size_t(size))) return false;
    if (out) out->assign(data.begin() + r.p, data.begin() + r.p + size);
    r.p += size_t(size);
    return true;
  };
  read_bulk(nullptr);  // source art
  if (r.I32() < 1) return false;
  Bytes mip;
  if (!read_bulk(&mip)) return false;
  const int32_t w = r.I32(), h = r.I32();
  if (!r.ok || w <= 0 || h <= 0) return false;
  const uint32_t wb = (uint32_t(w) + 3) / 4, hb = (uint32_t(h) + 3) / 4;
  const uint32_t pitch = (wb + 31) & ~31u, rows = (hb + 31) & ~31u;
  if (mip.size() < size_t(pitch) * rows * 16) return false;
  width = uint16_t(PropInt(props, "SizeX", w));
  height = uint16_t(PropInt(props, "SizeY", h));
  alpha.assign(size_t(width) * height, 0);
  for (uint32_t by = 0; by < hb; ++by) {
    for (uint32_t bx = 0; bx < wb; ++bx) {
      // Blocks are stored with 16-bit words byte-swapped.
      uint8_t block[8];
      const uint8_t* src = &mip[TiledOffset(bx, by, pitch, 4)];
      for (int i = 0; i < 8; i += 2) {
        block[i] = src[i + 1];
        block[i + 1] = src[i];
      }
      const uint32_t a0 = block[0], a1 = block[1];
      uint8_t palette[8] = {uint8_t(a0), uint8_t(a1)};
      for (int i = 2; i < 8; ++i) {
        palette[i] = a0 > a1 ? uint8_t(((8 - i) * a0 + (i - 1) * a1) / 7)
                     : i < 6 ? uint8_t(((6 - i) * a0 + (i - 1) * a1) / 5)
                             : uint8_t(i == 6 ? 0 : 255);
      }
      uint64_t bits = 0;
      for (int i = 0; i < 6; ++i) bits |= uint64_t(block[2 + i]) << (8 * i);
      for (uint32_t i = 0; i < 16; ++i) {
        const uint32_t x = bx * 4 + (i & 3), y = by * 4 + (i >> 2);
        if (x < width && y < height) alpha[size_t(y) * width + x] = palette[(bits >> (3 * i)) & 7];
      }
    }
  }
  return true;
}

// A UE3 bitmap Font (pages + per-character rects) -> .awf bytes.
bool ExportFont(const Package& pkg, const std::string& path, Bytes& out) {
  const Export* font = nullptr;
  for (const auto& e : pkg.exports) {
    if (pkg.FullPath(e) == path && pkg.ObjectName(e.class_index) == "Font") font = &e;
  }
  if (!font) return false;
  const Bytes data = pkg.Blob(*font);
  size_t end = 0;
  const auto props = ReadProperties(pkg, data, 4, &end);
  const auto chars_it = props.find("Characters"), tex_it = props.find("Textures");
  if (chars_it == props.end() || tex_it == props.end()) return false;
  struct Char {
    int32_t u, v, w, h;
    uint8_t page;
    int32_t y_offset;
  };
  std::vector<Char> chars;
  {
    Reader r{chars_it->second.raw};
    const int32_t n = r.I32();
    for (int32_t i = 0; i < n && r.ok; ++i) {
      Char c;
      c.u = r.I32();
      c.v = r.I32();
      c.w = r.I32();
      c.h = r.I32();
      c.page = r.U8();
      c.y_offset = r.I32();
      if (r.ok) chars.push_back(c);
    }
  }
  struct Page {
    uint16_t w = 0, h = 0;
    Bytes alpha;
  };
  std::vector<Page> pages;
  {
    Reader r{tex_it->second.raw};
    const int32_t n = r.I32();
    for (int32_t i = 0; i < n && r.ok; ++i) {
      const int32_t ref = r.I32();
      if (ref <= 0 || size_t(ref) > pkg.exports.size()) return false;
      Page page;
      if (!DecodeDxt5Alpha(pkg, pkg.exports[ref - 1], page.w, page.h, page.alpha)) return false;
      pages.push_back(std::move(page));
    }
  }
  // Native CharRemap (TMap<u16 char, u16 index>) follows the properties.
  std::map<uint32_t, uint32_t> remap;
  // IsRemapped is an IntProperty in these packages.
  const auto remapped = props.find("IsRemapped");
  if (remapped != props.end() && (remapped->second.flag || PropInt(props, "IsRemapped") != 0)) {
    Reader r{data, end};
    const int32_t n = r.I32();
    for (int32_t i = 0; i < n && r.Need(4); ++i) {
      remap[Be16(&data[r.p])] = Be16(&data[r.p + 2]);
      r.p += 4;
    }
  } else {
    for (uint32_t i = 0; i < chars.size(); ++i) remap[i] = i;
  }
  struct Glyph {
    uint32_t codepoint;
    Char c;
  };
  std::vector<Glyph> glyphs;
  int32_t line_height = 1;
  for (const auto& [cp, index] : remap) {
    if (index >= chars.size()) continue;
    glyphs.push_back({cp, chars[index]});
    if (chars[index].h > 0) line_height = std::max(line_height, chars[index].h + chars[index].y_offset);
  }
  const float ascent_f = PropFloat(props, "Ascent"), descent_f = PropFloat(props, "Descent");
  const int16_t ascent = int16_t(std::lround(
      ascent_f + descent_f > 0 ? line_height * ascent_f / (ascent_f + descent_f) : line_height));
  auto put16 = [&](uint32_t v) {
    out.push_back(uint8_t(v));
    out.push_back(uint8_t(v >> 8));
  };
  auto put32 = [&](uint32_t v) {
    put16(v & 0xFFFF);
    put16(v >> 16);
  };
  out.assign({'A', 'W', 'F', '1'});
  put16(uint32_t(pages.size()));
  put16(uint32_t(glyphs.size()));
  put16(uint16_t(int16_t(PropInt(props, "Kerning"))));
  put16(uint32_t(line_height));
  put16(uint16_t(ascent));
  put16(0);
  for (const auto& page : pages) {
    put16(page.w);
    put16(page.h);
  }
  for (const auto& g : glyphs) {
    put32(g.codepoint);
    put16(g.c.page);
    put16(uint32_t(g.c.u));
    put16(uint32_t(g.c.v));
    put16(uint32_t(g.c.w));
    put16(uint32_t(g.c.h));
    put16(uint16_t(int16_t(g.c.y_offset)));
  }
  for (const auto& page : pages) out.insert(out.end(), page.alpha.begin(), page.alpha.end());
  return true;
}

// .xxx -> plain package body (as tools/xxx_unpack does).
bool UnpackXxx(const fs::path& path, Bytes& body) {
  Bytes file;
  if (!ReadFile(path, file) || file.size() < 16) return false;
  // Packages whose summary is stored uncompressed keep it before the first
  // compressed chunk; the chunk table says where that one starts.
  size_t body_offset = 0;
  if (Be32(file.data()) == 0x9E2A83C1 && (Be32(&file[4]) & 0xFFFF) == 841) {
    size_t p = 12;
    const int32_t folder_len = int32_t(Be32(&file[p]));
    p += 4 + (folder_len < 0 ? size_t(-folder_len) * 2 : size_t(folder_len));
    p += 4 + 6 * 4 + 4 + 3 * 4 + 4 + 16;
    if (p + 4 <= file.size()) {
      const uint32_t generations = Be32(&file[p]);
      p += 4 + size_t(generations) * 12 + 12;
      if (p + 8 <= file.size()) {
        const uint32_t chunks = Be32(&file[p]);
        if (chunks && chunks <= 4096) body_offset = Be32(&file[p + 4]);
      }
    }
  }
  body.assign(file.begin(), file.begin() + std::min(body_offset, file.size()));
  mantra_shadergen::PackageStats stats;
  return mantra_shadergen::DecompressPackage(file, body, stats);
}

}  // namespace

fs::path GameCacheRoot() { return platform::CacheDir(); }

fs::path UiCacheDir() {
  const auto root = GameCacheRoot();
  return root.empty() ? root : root / "ui";
}

fs::path AchievementCacheDir() {
  const auto root = GameCacheRoot();
  return root.empty() ? root : root / "achievements";
}

bool ExtractUiFonts(const fs::path& game_dir, const fs::path& out_dir, std::string* error) {
  std::error_code ec;
  fs::create_directories(out_dir, ec);
  // The international package's fonts (required), then copies from the
  // localized ones for the accents and Japanese it lacks (optional).
  std::vector<std::string> languages = {"INT"};
  for (const char* language : kGameFontLanguages) languages.emplace_back(language);
  for (const std::string& language : languages) {
    const bool required = language == "INT";
    Package pkg;
    const fs::path source =
        game_dir / "BCGame" / "CookedXbox360" / ("Startup_LOC_" + language + ".xxx");
    if (!UnpackXxx(source, pkg.data) || !ParsePackage(pkg)) {
      if (!required) continue;
      Fail(error, "cannot read " + source.string());
      return false;
    }
    const std::string prefix = required ? "UI_Fonts." : "UI_Fonts_" + language + ".";
    for (const char* name : {"MediumFont", "TitleFont"}) {
      Bytes awf;
      if (!ExportFont(pkg, prefix + name, awf)) {
        if (!required) continue;
        Fail(error, std::string("cannot convert the game font ") + name);
        return false;
      }
      // Write next to it, then rename, so a reader never sees half a file.
      const fs::path target =
          out_dir / (std::string(name) + (required ? "" : "_" + language) + ".awf");
      fs::path temporary = target;
      temporary += ".tmp";
      if (!WriteFile(temporary, awf.data(), awf.size())) {
        Fail(error, "cannot write " + target.string());
        return false;
      }
      fs::rename(temporary, target, ec);
    }
  }
  const std::string version = kUiFontsVersion;
  return WriteFile(out_dir / "fonts.version", reinterpret_cast<const uint8_t*>(version.data()),
                   version.size());
}

bool HaveExtractedUiFonts() {
  std::ifstream in(UiCacheDir() / "fonts.version");
  std::string version;
  return in && std::getline(in, version) && version == kUiFontsVersion;
}

bool ExtractAchievementList(const fs::path& game_dir, const fs::path& out_dir, uint32_t language,
                            std::string* error) {
  Bytes xex;
  if (!ReadFile(game_dir / "default.xex", xex)) {
    Fail(error, "cannot read default.xex");
    return false;
  }
  XexImage image;
  if (!LoadXex(xex, image, error)) return false;
  const auto resource = XexResource(xex, image, "43430817");
  if (!resource) {
    Fail(error, "default.xex has no achievement data");
    return false;
  }
  const rex::system::util::XdbfGameData db(resource->first, resource->second);
  if (!db.is_valid()) {
    Fail(error, "default.xex achievement data is damaged");
    return false;
  }
  // A translation: the game's language, with the translation's names on top.
  const auto lang = db.GetExistingLanguage(rex::system::XLanguage(GameLanguage(language)));
  std::error_code ec;
  fs::create_directories(out_dir, ec);
  auto clean = [](std::string s) {
    std::replace_if(s.begin(), s.end(), [](char c) { return c == '\t' || c == '\n' || c == '\r'; },
                    ' ');
    return s;
  };
  std::string tsv = "id\tgamerscore\tname\tdescription\tlocked_description\n";
  for (const auto& a : db.GetAchievements()) {
    const uint32_t id = a.id;
    std::string name = db.GetStringTableEntry(lang, a.label_id);
    std::string description = db.GetStringTableEntry(lang, a.description_id);
    std::string locked = db.GetStringTableEntry(lang, a.unachieved_id);
    if (const TranslatedAchievement t = TranslatedAchievementText(language, id); !t.name.empty()) {
      name = t.name;
      description = locked = t.description;
    }
    tsv += std::to_string(id) + '\t' + std::to_string(uint32_t(a.gamerscore)) + '\t' +
           clean(name) + '\t' + clean(description) + '\t' + clean(locked) + '\n';
    if (const auto icon = db.GetEntry(rex::system::util::XdbfSection::kImage, a.image_id)) {
      WriteFile(out_dir / (std::to_string(id) + ".png"), icon.buffer, icon.size);
    }
  }
  const fs::path target = out_dir / "achievements.tsv";
  fs::path temporary = target;
  temporary += ".tmp";
  if (!WriteFile(temporary, tsv.data(), tsv.size())) {
    Fail(error, "cannot write " + target.string());
    return false;
  }
  fs::rename(temporary, target, ec);
  const std::string lang_text = std::to_string(language);
  WriteFile(out_dir / "language", lang_text.data(), lang_text.size());
  return true;
}

uint32_t CachedAchievementLanguage() {
  std::ifstream in(AchievementCacheDir() / "language");
  uint32_t language = 0;
  std::error_code ec;
  if (!(in >> language) || !fs::is_regular_file(AchievementCacheDir() / "achievements.tsv", ec)) {
    return 0;
  }
  return language;
}

}  // namespace asura::ui
