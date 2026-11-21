#include "save_data.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <map>
#include <memory>
#include <mutex>

#include "translation.h"

namespace asura::ui {

namespace {

std::vector<uint8_t> ReadFile(const std::filesystem::path& path) {
  std::ifstream in(path, std::ios::binary);
  if (!in) return {};
  return std::vector<uint8_t>(std::istreambuf_iterator<char>(in), {});
}

// --- UE3 tagged properties (save) ------------------------------------------------------

struct Reader {
  const uint8_t* data;
  size_t size, pos;
  bool ok = true;
  uint32_t U32() {
    if (pos + 4 > size) { ok = false; return 0; }
    const uint32_t v = uint32_t(data[pos]) << 24 | uint32_t(data[pos + 1]) << 16 |
                       uint32_t(data[pos + 2]) << 8 | data[pos + 3];
    pos += 4;
    return v;
  }
  // Inline FString (ANSI, with its terminating zero).
  std::string String() {
    const uint32_t n = U32();
    if (!ok || n == 0 || n > 1024 || pos + n > size) { ok = false; return {}; }
    std::string s(reinterpret_cast<const char*>(data + pos), n - 1);
    pos += n;
    return s;
  }
};

struct Prop {
  std::string name, type, tag;  // tag: enum or struct name
  int index = 0;
  int32_t int_value = 0;
  float float_value = 0;
  std::string name_value;               // ByteProperty enum value
  std::vector<std::vector<Prop>> items;  // ArrayProperty of structs
  std::vector<int32_t> ints;             // ArrayProperty of ints
  std::vector<Prop> fields;              // StructProperty
};

std::vector<Prop> ReadProps(Reader& r, size_t end, int depth);

bool ReadProp(Reader& r, size_t end, int depth, Prop* p) {
  p->name = r.String();
  if (!r.ok || p->name == "None") return false;
  p->type = r.String();
  const uint32_t size = r.U32();
  p->index = int(r.U32());
  if (!r.ok) return false;
  if (p->type == "ByteProperty") {
    p->tag = r.String();
    p->name_value = r.String();
    return r.ok;
  }
  if (p->type == "StructProperty") p->tag = r.String();
  const size_t start = r.pos;
  if (size > end - std::min(end, start)) { r.ok = false; return false; }
  Reader v{r.data, start + size, start};
  if (p->type == "IntProperty" || p->type == "BoolProperty") {
    p->int_value = int32_t(v.U32());
  } else if (p->type == "FloatProperty") {
    const uint32_t bits = v.U32();
    std::memcpy(&p->float_value, &bits, 4);
  } else if (p->type == "StructProperty" && depth < 8) {
    p->fields = ReadProps(v, start + size, depth + 1);
  } else if (p->type == "ArrayProperty" && depth < 8) {
    const uint32_t count = v.U32();
    if (size == 4 + count * 4) {
      for (uint32_t i = 0; i < count; ++i) p->ints.push_back(int32_t(v.U32()));
    } else {
      for (uint32_t i = 0; i < count && v.ok; ++i) p->items.push_back(ReadProps(v, start + size, depth + 1));
    }
  }
  r.pos = start + size;
  return true;
}

std::vector<Prop> ReadProps(Reader& r, size_t end, int depth) {
  std::vector<Prop> props;
  Prop p;
  while (r.ok && r.pos < end && ReadProp(r, end, depth, &p)) {
    props.push_back(std::move(p));
    p = Prop{};
  }
  return props;
}

const Prop* Find(const std::vector<Prop>& props, const char* name, int index = 0) {
  for (const auto& p : props) {
    if (p.name == name && p.index == index) return &p;
  }
  return nullptr;
}

int IntOf(const std::vector<Prop>& props, const char* name, int index = 0) {
  const Prop* p = Find(props, name, index);
  return p ? p->int_value : 0;
}

// "BCRRank_S" -> "S", "BCRRank_None" -> "".
std::string RankOf(const Prop* p) {
  if (!p) return {};
  const std::string& v = p->name_value;
  const size_t sep = v.rfind('_');
  const std::string rank = sep == std::string::npos ? v : v.substr(sep + 1);
  return rank == "None" ? std::string() : rank;
}

// --- Coalesced_<LANG>.bin -----------------------------------------------------------

const char* LanguageCode(uint32_t language) {
  switch (language) {
    case 2: return "JPN";
    case 3: return "DEU";
    case 4: return "FRA";
    case 5: return "ESN";
    case 6: return "ITA";
    default: return "INT";
  }
}

void AppendUtf8(std::string& out, uint32_t c) {
  if (c < 0x80) {
    out += char(c);
  } else if (c < 0x800) {
    out += char(0xC0 | (c >> 6));
    out += char(0x80 | (c & 0x3F));
  } else {
    out += char(0xE0 | (c >> 12));
    out += char(0x80 | ((c >> 6) & 0x3F));
    out += char(0x80 | (c & 0x3F));
  }
}

// What the launcher needs from one Coalesced file.
struct Coalesced {
  // MapProvider_<id> BCUIDataProvider_ChapterInfo -> key -> value.
  std::map<std::string, std::map<std::string, std::string>> chapters;
  std::map<std::string, std::string> titles;  // "S000" -> title
};

bool ParseCoalesced(const std::vector<uint8_t>& d, Coalesced* out) {
  size_t pos = 0;
  bool ok = true;
  auto i32 = [&]() -> int32_t {
    if (pos + 4 > d.size()) { ok = false; return 0; }
    const int32_t v = int32_t(uint32_t(d[pos]) << 24 | uint32_t(d[pos + 1]) << 16 |
                              uint32_t(d[pos + 2]) << 8 | d[pos + 3]);
    pos += 4;
    return v;
  };
  // FString: positive length = ANSI, negative = UTF-16LE; both end with a zero.
  auto fstring = [&]() -> std::string {
    const int32_t n = i32();
    std::string s;
    if (!ok || n == 0) return s;
    if (n > 0) {
      if (size_t(n) > d.size() - pos) { ok = false; return s; }
      s.assign(reinterpret_cast<const char*>(&d[pos]), size_t(n));
      pos += size_t(n);
    } else {
      const size_t chars = size_t(-int64_t(n));
      if (chars * 2 > d.size() - pos) { ok = false; return s; }
      for (size_t i = 0; i < chars; ++i) {
        AppendUtf8(s, uint32_t(d[pos + i * 2]) | uint32_t(d[pos + i * 2 + 1]) << 8);
      }
      pos += chars * 2;
    }
    while (!s.empty() && s.back() == '\0') s.pop_back();
    return s;
  };
  const int32_t files = i32();
  for (int32_t f = 0; ok && f < files; ++f) {
    const std::string file = fstring();
    std::string lower = file;
    std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);
    const bool game_ini = lower.size() >= 10 && lower.ends_with("bcgame.ini");
    const bool strings = lower.find("_defaultfont") == std::string::npos &&
                         lower.find("\\bcgame.") != std::string::npos && !game_ini;
    const int32_t sections = i32();
    for (int32_t s = 0; ok && s < sections; ++s) {
      const std::string section = fstring();
      const bool chapter = game_ini && section.find("BCUIDataProvider_ChapterInfo") != std::string::npos;
      const bool title = strings && section == "Title";
      const int32_t keys = i32();
      for (int32_t k = 0; ok && k < keys; ++k) {
        std::string key = fstring();
        std::string value = fstring();
        if (chapter) out->chapters[section][key] = std::move(value);
        if (title) out->titles[key] = std::move(value);
      }
    }
  }
  return ok && !out->chapters.empty();
}

// Parsed files, by path (the launcher asks every frame while the page is open).
const Coalesced* LoadCoalesced(const std::filesystem::path& game_root, uint32_t language) {
  static std::mutex mutex;
  static std::map<std::string, std::unique_ptr<Coalesced>> cache;
  // A translation's own copy when it has one (translation.h).
  const std::string path =
      GameFileFor(game_root, language,
                  std::filesystem::path("BCGame") / "CookedXbox360" /
                      (std::string("Coalesced_") + LanguageCode(GameLanguage(language)) + ".bin"))
          .string();
  std::lock_guard lock(mutex);
  auto it = cache.find(path);
  if (it == cache.end()) {
    auto parsed = std::make_unique<Coalesced>();
    if (!ParseCoalesced(ReadFile(path), parsed.get())) parsed.reset();
    it = cache.emplace(path, std::move(parsed)).first;
  }
  if (!it->second && language != 1) return LoadCoalesced(game_root, 1);
  return it->second.get();
}

}  // namespace

bool StoryRecord::Cleared() const {
  if (clear_flag & 4) return true;
  return std::any_of(rank.begin(), rank.end(), [](const std::string& r) { return !r.empty(); });
}

bool ReadSaveSummary(const std::filesystem::path& file, SaveSummary* out) {
  const std::vector<uint8_t> d = ReadFile(file);
  if (d.size() < 16) return false;
  // u32 0, u32 payload size, then the properties.
  Reader r{d.data(), d.size(), 8};
  const std::vector<Prop> props = ReadProps(r, d.size(), 0);
  const Prop* stories = Find(props, "StoryClearFlgs");
  if (!stories) return false;
  *out = SaveSummary{};
  for (const auto& item : stories->items) {
    StoryRecord record;
    record.id = IntOf(item, "Id");
    record.clear_flag = IntOf(item, "ClearFlg");
    if (const Prop* results = Find(item, "ResultInfo")) {
      for (size_t i = 0; i < results->items.size() && i < kDifficultyCount; ++i) {
        record.rank[i] = RankOf(Find(results->items[i], "TotalRank"));
      }
    }
    out->stories.push_back(std::move(record));
  }
  if (const Prop* system = Find(props, "SystemParam")) {
    if (const Prop* diff = Find(system->fields, "GameDifficulty")) {
      const std::string& v = diff->name_value;
      if (v.ends_with("_Easy")) out->difficulty = kEasy;
      if (v.ends_with("_Normal")) out->difficulty = kNormal;
      if (v.ends_with("_Hard")) out->difficulty = kHard;
    }
  }
  out->battle_points = IntOf(props, "SumBattlePoint");
  out->enemies_defeated = IntOf(props, "KillEnemyNum", 0) + IntOf(props, "KillEnemyNum", 1);
  out->continues = IntOf(props, "ContinueNum");
  return true;
}

std::vector<EpisodeInfo> ReadEpisodes(const std::filesystem::path& game_root, uint32_t language) {
  std::vector<EpisodeInfo> episodes;
  const Coalesced* c = LoadCoalesced(game_root, language);
  if (!c) return episodes;
  for (const auto& [section, keys] : c->chapters) {
    auto get = [&](const char* key) {
      auto it = keys.find(key);
      return it == keys.end() ? std::string() : it->second;
    };
    if (get("bDLC") == "TRUE") continue;
    EpisodeInfo e;
    e.id = std::atoi(get("Id").c_str());
    e.part = std::atoi(get("ChapterNo").c_str());
    e.number = get("StoryNo");
    e.title = EpisodeTitle(game_root, language, e.id);
    episodes.push_back(std::move(e));
  }
  std::sort(episodes.begin(), episodes.end(),
            [](const EpisodeInfo& a, const EpisodeInfo& b) { return a.id < b.id; });
  return episodes;
}

std::string EpisodeTitle(const std::filesystem::path& game_root, uint32_t language, int id) {
  const Coalesced* c = LoadCoalesced(game_root, language);
  if (!c) return {};
  char key[16];
  std::snprintf(key, sizeof(key), "S%03d", id);
  auto it = c->titles.find(key);
  return it == c->titles.end() ? std::string() : it->second;
}

}  // namespace asura::ui
